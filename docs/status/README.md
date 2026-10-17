# Workstreams

The lead's index (the architect-gated process in `docs/PLAN.md`): every live workstream, its
branch and worktree, who drives it, its state, and its next step. Each implementer keeps its
own ledger at `docs/status/<workstream>.md` on its branch; after a lost session, read this
file, then each ledger and its branch's `git log`.

| Workstream | Repo | Branch | Worktree | Agent | State | Next | Updated |
|---|---|---|---|---|---|---|---|
| Phase 3 gate | strata | main | `strata` | lead | measuring | dry run clean; `night_gate.sh n1017` starts 23:00 under `quiet_trials.sh` (strict hold); five-trial query rows the next night; then the gate review | 2026-10-17 |
| Format 13 | strata | `phase4/format13` | `strata-wt/format13` | implementer | designing | ledger `docs/status/format13.md`; draft `docs/design/format-13.md`, then the architect's preliminary review; implement the stable decisions | 2026-10-17 |
| Layers refactor | strata | `phase4/layers-refactor` | `strata-wt/refactor` | implementer | implementing | ledger `docs/status/layers-refactor.md`; rebase onto main (P1); step 5 | 2026-10-17 |
| Xapiand failover | Xapiand | `fix/failover` (local) | `Xapiand-wt/failover` | implementer | investigating | writes stay 503 120 s after the leader dies, at the old and new pins alike; find the cause and fix | 2026-10-17 |
| Xapiand HTTPS (D1) gate | Xapiand | `tls/d1-gate` (local) | `Xapiand-wt/tls-gate` | implementer | implementing | T1, T2 pass (`7e8cccb7d`); T3 probe fix, C5 counters, C6 daily expiry, T4 harness, gate report; the lead measures T4 and pushes for T5 | 2026-10-17 |
| Track D1 simulator | strata | `design/simulator` | `strata-wt/simulator` | implementer | designing | ledger `docs/status/simulator.md`; `docs/design/simulator.md` (scale contract, deterministic simulator, shared with C5); preliminary review; first steps | 2026-10-17 |
| Phase 5 automata | strata | `phase5/automata` | `strata-wt/automata` | implementer | designing | ledger `docs/status/automata.md`; `docs/design/automata.md`; preliminary review; implement with brute-force checks | 2026-10-17 |
| PKI helper | Xapiand | `design/pki` (local) | `Xapiand-wt/pki` | implementer | designing | ledger `docs/status/pki.md`; `docs/design/pki.md` for Xapiand and Detent; preliminary review; owner's open questions | 2026-10-17 |

## Landed

- P1 reordering: on main through `950d24d` (T1 to T7 recorded in `docs/gates/phase-3-ratchets.md`
  and `docs/design/reordering.md`).
- P6 multi-segment search: every commit of `phase3/p6-multisegment` is on main (`9e016ca` through
  `94fffcc`; `5c199dd` is `8fefcb4`, the same diff on newer context). Branch and worktree removed.
