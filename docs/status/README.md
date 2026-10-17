# Workstreams

The lead's index (the architect-gated process in `docs/PLAN.md`): every live workstream, its
branch and worktree, who drives it, its state, and its next step. Each implementer keeps its
own ledger at `docs/status/<workstream>.md` on its branch; after a lost session, read this
file, then each ledger and its branch's `git log`.

| Workstream | Repo | Branch | Worktree | Agent | State | Next | Updated |
|---|---|---|---|---|---|---|---|
| Phase 3 gate | strata | main | `strata` | lead | measuring | T4 and T3 tonight (the gate VM); the five-trial query rows the next night; then the gate review | 2026-10-17 |
| Format 13 | strata | `phase4/format13` | `strata-wt/format13` | implementer | designing | ledger `docs/status/format13.md`; draft `docs/design/format-13.md`, then the architect's preliminary review; implement the stable decisions | 2026-10-17 |
| Layers refactor | strata | `phase4/layers-refactor` | `strata-wt/refactor` | implementer | implementing | ledger `docs/status/layers-refactor.md`; rebase onto main (P1); step 5 | 2026-10-17 |
| p6 leftover | strata | `phase3/p6-multisegment` | `strata-wt/p6` | lead | triage | `5c199dd` (R5, dead counts in snapshots) is not on main by patch: land it or record why not | 2026-10-17 |
| Xapiand failover | Xapiand | `fix/failover` (local) | `Xapiand-wt/failover` | implementer | investigating | writes stay 503 120 s after the leader dies, at the old and new pins alike; find the cause and fix | 2026-10-17 |
| Xapiand HTTPS (D1) | Xapiand | master (local) | `Xapiand` | lead | implemented | gate rows T1 to T7, then the step-0 CI run on Linux once pushed | 2026-10-17 |

## Landed

- P1 reordering: on main through `950d24d` (T1 to T7 recorded in `docs/gates/phase-3-ratchets.md`
  and `docs/design/reordering.md`).
