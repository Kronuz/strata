# peer-transport

- Design: `docs/design/peer-transport.md`, revision 1 at `90d417e` (revision 0 was `8a1f840`,
  `cb893e8` after the rebase). Reviews of revision 0, both 2026-10-17
  and APPROVE WITH CONDITIONS: the architect's preliminary review
  (`docs/reviews/peer-transport-design.md`, PT-R1 to PT-R19) and the security reviewer's
  (`docs/reviews/peer-transport-security.md`, SR1 to SR14). Due: both reviewers' re-checks of
  revision 1, appended to those files.
- Branch / worktree: `design/peer-transport` at `~/code/Kronuz/strata-wt/peer-transport`, rebased
  onto `main` at `c5b571a` on 2026-10-17.
- State: designing (ready for preliminary review)
- Done criteria (drafted by the designer at the lead's request, from the architect's condition
  C1; the lead confirms or edits them; contributors tick, never edit):
  - [ ] K1 Step 1 (pins, `Directory`, `AddressBook`, `authenticated_node`): T1's cases and T2's
    invariant cases pass on all eight `consensus` CI jobs; the old functions keep their
    behavior, shown by Detent's authentication cases passing through them.
  - [ ] K2 Step 2 (the relocation): the characterization test committed at the pre-relocation
    commit and passing unchanged after it (T5, PT-R9); T4 over today's rules with each seed's
    action hash pinned; T5's replay; `Peers`' API, `PeerStats`, `PeerOccupancy`, and
    `AppendTimings` unchanged.
  - [ ] K3 Step 3 (the detector): its T4 cases (down at exactly 3k of silence and after three
    unanswered `Ping`s, never earlier; up at the first frame; `restarted` on a new boot
    identity; one pending `Pong`), with the `Peers` preset ignoring its reports.
  - [ ] K4 Step 5 (the binding): D18's check at append and at recovery, its mutant caught;
    `Core::known_members()`; D5's rules with T7's cases (SR1's, a same-node replacement,
    conflicting uncommitted entries, a removed leader's hand-off, leader-only kinds from vouched
    sources, a compromised leader's moving entry); the binding-off mutant caught within 400 seeds
    in T2's harness; SR4's admission rule with its T7 case; fixed groups unchanged.
  - [ ] K5 Step 6 (reload): generations re-verifying every older session, a handshake that
    straddles the swap included; `close_at_expiry` off by default and closing at notAfter when
    on; reactor's PEM-in-memory and TLS 1.3-under-`strict` changes with reactor's own tests and
    T1's TLS 1.2 refusal; SYS-13's key-file checks.
  - [ ] K6 ASan, UBSan, and TSan over every transport test; line and branch coverage of the
    changed headers at TESTING's targets (decoders 98% and 95%, the rest 90% and 80%).
  - [ ] K7 Every step's commit green on all eight `consensus` CI jobs (and reactor's for its
    change), with Detent's transport suite run at that commit with the pin moved in a scratch
    build and recorded in Detent's `LIBRARY_BOUNDARIES.md` through the lead (Track C's
    condition 1; the architect's C3).
  - [ ] K8 Step 4 (version 2's wire) lands only after the full design review and the security
    review clear it, C2's bodies for kinds 9 to 12 are fixed, and Detent's agreement to Q4 is
    recorded in its `DEPENDENCY_UPGRADES.md`; on its branch T3, T6's functional checks, T8's
    fuzz targets, and T13 pass.
  - [ ] K9 Step 7 (the simulator's adapter): the adapter drives `PeerPolicy`, so the simulator's
    refusals by pin and by version gap are the policy's (T5's adapter half).
- Done:
  - Revision 0: D1 to D15, the threat model, the cost and limits tables, T1 to T12, Q1 to Q5,
    and seven first steps (`8a1f840`, now `cb893e8`).
  - Rebased onto `origin/main` `c5b571a` (ten docs-only commits, no conflict) and force-pushed
    with lease.
  - Revision 1: every finding of both reviews closed where its ID appears; the owner's decisions
    on Q1 to Q5 with the reviewers' conditions; new D16 (certified directory versions), D17 (the
    admin path), and D18 (every replica checks membership changes); the threat model rewritten;
    T13; Q6; a "Changes from revision 0" table; the items Detent must hear; first steps with
    what each waits on (`90d417e`). Arithmetic in `~/.scratch/peer-transport-r1/arith.py` and
    `arith.out`.
- Doing: nothing; stopped at ready for the reviewers' re-checks, as the assignment asks.
- Next:
  1. The lead runs both re-checks of revision 1, the architect's and the security reviewer's,
     appended to their review files.
  2. The owner's answer to Q6 (a staleness deadline), through the lead.
  3. The lead adds the design's "For Detent" items to
     `~/code/Detent/docs/SHARED_LIBRARY_DECISIONS.md` and applies the "Changes to other
     documents" rows on `main`.
  4. A developer starts first steps 1, 2 (the characterization test first), 3, 5, and 6 on
     `consensus` branches, step 6's reactor change in reactor first, and may draft step 4's
     frame header and kinds 13, 14, and 16; builds wait for the host hold.
  5. Step 4's hello, `Window`, `Fragment`, and flow control after the re-check; kinds 9 to 12
     after C2's bodies.
- Blocked on:
  - Q6 (the owner); nothing before step 8 waits on it.
  - Q4's confirmation by Detent's maintainer, in Detent's `DEPENDENCY_UPGRADES.md`, before step 4
    lands.
  - C2's bodies for kinds 9 to 12 (Track C's D12), before version 2's kinds freeze.
  - Track D3's placement records carrying incarnations, and a root without a witness, before
    strata's witness and D16's code (step 8).
- Findings folded in: SYS-2, SYS-3, SYS-12, SYS-14 (`docs/reviews/system-2026-10-17.md`) and the
  owner's rulings on them (`docs/PLAN.md:26-33, 45-46`); the simulator's Q3, SIM-R6, and SIM-R7;
  transport-security's TS-R2 to TS-R8, TS-R12, and TS-R13; the PKI design's D5, D10, SR12, and
  its Q6 as decided; Track C's R12 and R19 (revision 0). PT-R1 to PT-R19 and SR1 to SR14, with
  the architect's conditions C1 to C5 and the security reviewer's C1 to C9, and the decisions on
  Q1 to Q5 (revision 1, `90d417e`).
- Last verified: 2026-10-17. Nothing compiled or run: design only, and `quiet check` exits 1 (the
  strata-trials strict hold until 01:17). Only the arithmetic probe ran, in Python at nice 19.
  Every file:line in revision 1 was read at the commit its header names.

## Owner decisions on Q1 to Q5 (2026-10-17)

Made by the lead under the owner's standing permission for educated calls; both reviews
recommend option 1 on every question, with conditions that revision 1 folds in:

- Q1: the root group's node table, with quorum-signed versions any node relays (PT-R7, SR2)
  and an authentication-only `Directory`.
- Q2: the group's parent authority (the placement record) as witness, with SR1's tightenings
  (one incarnation per node per group, `Vouch{group, node, incarnation}`), followers
  validating every membership change (PT-R10), and vouches expiring with their record.
- Q3: membership gossip as host frames on this transport, with hardened handshake admission
  (SR7, PT-R13) and a fixed probing topology.
- Q4: Detent leaves version 1 by one coordinated restart, adopting version 2 itself rather
  than any intermediate commit; Detent's maintainer confirms.
- Q5: online enrollment is its own security-tier design, with an owner and a trigger named in
  revision 1 and the directory side defined now (PT-R19, SR3).

## Open for the owner

- Q6 (revision 1, SR9): whether a node whose directory is stale past a deadline refuses new
  sessions. Recommendation, with the security reviewer's: no deadline, the residual stated.
