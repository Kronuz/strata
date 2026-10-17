# peer-transport

- Design: `docs/design/peer-transport.md`, revision 0 at `8a1f840`;
  reviews: none yet. Due: the architect's preliminary review
  (`docs/reviews/peer-transport-design.md`) and, since the design sets authentication, TLS,
  and a wire protocol, the security reviewer's review of its threat model
  (`docs/reviews/peer-transport-security.md`).
- Branch / worktree: `design/peer-transport` at `~/code/Kronuz/strata-wt/peer-transport`, from
  `main` at `5691ec7`.
- State: designing (ready for preliminary review)
- Done criteria (set by the lead; contributors tick, never edit):
  - None yet; the lead writes them with the preliminary review.
- Done:
  - Read the inputs the assignment names, at the commits the design's header lists: PLAN's
    process, Track C, Track D, and the owner's decisions; `track-c.md` and its review;
    the system review's SYS-2, SYS-3, SYS-12, and SYS-14; DISTRIBUTION; the simulator's design
    and review (`design/simulator`); Xapiand's transport-security design and review; the PKI
    design and its reviews (`design/pki`, with the owner's decisions on its revision 1);
    consensus's `transport/` headers, `core.h`, `worker.h`, `membership.h`, and `types.h`;
    `reactor_tls.h`; Detent's `SHARED_LIBRARY_DECISIONS.md`, `AUTHORITY_TRANSPORT.md`,
    `PERSISTENT_TRANSPORT_ARCHITECTURE.md`, and its adoption notes, read only.
  - Wrote revision 0: D1 to D15, the threat model, the cost and limits tables, T1 to T12,
    Q1 to Q5, and seven first steps (`8a1f840`).
- Doing: nothing; stopped at ready for preliminary review, as the assignment asks.
- Next:
  1. The architect's preliminary review and the security reviewer's review of the threat
     model, run by the lead.
  2. The owner's answers to Q1 to Q5. Q4 and the `consensus::transport` changes (D2, D9) also
     need Detent's maintainer, through Detent's agent.
  3. Fold the findings in as a revision 1, citing their IDs.
  4. Then first steps 1 to 3, which no question blocks, built ahead on a branch of
     `consensus` once the lead assigns a developer; step 4 (version 2's wire) lands only after
     the full design review and the security review clear it.
- Blocked on:
  - Q1 to Q5 (the owner); Q4 with Detent's maintainer.
  - Step 4's tags 9 to 12 need C2's bodies (Track C's D12) fixed first, since every version-2
    tag is fixed before the coordinated `DTC2` commit.
- Findings folded in: SYS-2, SYS-3, SYS-12, SYS-14 (`docs/reviews/system-2026-10-17.md`) and
  the owner's rulings on them (`docs/PLAN.md:26-33, 45-46`); the simulator's Q3, SIM-R6, and
  SIM-R7; transport-security's TS-R2 to TS-R8, TS-R12, and TS-R13; the PKI design's D5, D10,
  SR12, and its Q6 as decided (enrollment here); Track C's R12 and R19.
- Last verified: 2026-10-17. Nothing compiled or run: design only, and `quiet check` exits 1
  (the strata-trials strict hold until 01:17). Every file:line in the design was read at the
  commit its header names.

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
