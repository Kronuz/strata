# Workstreams

The lead's index (the gated pipeline in `docs/PLAN.md`): every live workstream, its
branch and worktree, who drives it, its state, and its next step. Each implementer keeps its
own ledger at `docs/status/<workstream>.md` on its branch; after a lost session, read this
file, then each ledger and its branch's `git log`.

| Workstream | Repo | Branch | Worktree | Agent | State | Next | Updated |
|---|---|---|---|---|---|---|---|
| Phase 3 gate | strata | main | `strata` | lead | measuring | dry run clean; `night_gate.sh n1017` starts 23:00 under `quiet_trials.sh` (strict hold); five-trial query rows the next night; then the gate review | 2026-10-17 |
| Format 13 | strata | `phase4/format13` | `strata-wt/format13` | contributor | designing | ledger `docs/status/format13.md`; draft `docs/design/format-13.md`, then the architect's preliminary review; implement the stable decisions | 2026-10-17 |
| Layers refactor | strata | `phase4/layers-refactor` | `strata-wt/refactor` | contributor | implementing | ledger `docs/status/layers-refactor.md`; rebase onto main (P1); step 5 | 2026-10-17 |
| Xapiand failover | Xapiand | `fix/failover` (local) | `Xapiand-wt/failover` | contributor | fixing | `8a5bd916a` (promote the best live replica) passes FAILOVER=1 cluster_check 3 three times of three; `c51da3e09` (handler exceptions) unbuilt; the full matrix after the gate run; owner: accept promoting an empty replica? | 2026-10-17 |
| Xapiand HTTPS (D1) gate | Xapiand | `tls/d1-gate` (local) | `Xapiand-wt/tls-gate` | contributor | implementing | T1, T2 pass (`7e8cccb7d`); T3 probe fix, C5 counters, C6 daily expiry, T4 harness, gate report; the lead measures T4 and pushes for T5 | 2026-10-17 |
| Track D1 simulator | strata | `design/simulator` | `strata-wt/simulator` | contributor | implementing, in preliminary review | design at `e305adb`, S1 engine at `b70f2b4`; the architect reviews (`simulator-design.md`) while S3 (checker) and S2 (network) proceed; owner's Q1 to Q6 after the review | 2026-10-17 |
| Phase 5 automata | strata | `phase5/automata` | `strata-wt/automata` | contributor | designing | ledger `docs/status/automata.md`; `docs/design/automata.md`; preliminary review; implement with brute-force checks | 2026-10-17 |
| PKI helper | Xapiand | `design/pki` (local) | `Xapiand-wt/pki` | contributor | in preliminary review | rev 0 at `58aa0c7ee`; the architect (`pki-design.md`) and the security reviewer (`pki-security.md`) in parallel; then the owner's Q1 to Q6 | 2026-10-17 |

## Queued

Paused on 2026-10-17 at the usage limit, to resume as `quota advise` allows, in this order (each
from its ledger, as the agent type named):

1. Format 13 (`designer`, then `developer`): the design draft, then the preliminary review.
2. Track D1 simulator (`developer`): CI on `7d0b3fa`, then S4 once its review lands.
3. Xapiand HTTPS gate (`developer`): T3, C5, C6, the T4 harness, the gate report.
4. Phase 5 automata (`developer`): the design's preliminary review, then the rest of the build.
5. Layers refactor (`developer`): the sanitizer run and CI, then step 5.
6. reactor UDP receive loop (`developer`, in `~/code/Kronuz/reactor`): the loop exits for good
   when a handler throws, so a failover right after the first write never elects a leader
   (found by Xapiand failover, `c51da3e09`). The fix must be additive, since Detent links reactor.

The gate VM run (`night_gate.sh n1017`, started 13:17) holds the host strictly until about
01:17; resumed agents draft and push without compiling until it ends.

## Landed

- P1 reordering: on main through `950d24d` (T1 to T7 recorded in `docs/gates/phase-3-ratchets.md`
  and `docs/design/reordering.md`).
- P6 multi-segment search: every commit of `phase3/p6-multisegment` is on main (`9e016ca` through
  `94fffcc`; `5c199dd` is `8fefcb4`, the same diff on newer context). Branch and worktree removed.
