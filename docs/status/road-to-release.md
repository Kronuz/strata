# Road to the single-node release

> Status: CURRENT (2026-11-01). The lead's plan from Phase 7c's state to the first Xapian-free
> release (Phase 8 at `rf = 1`), and the strata lane beside it. Owner rulings are in
> `xapiand-on-strata.md`; Xapiand's own ledgers are in its `docs/status/phase7c*.md` (local master).

## Where it stands

- Phase 7a passed (2026-10-29). Phase 7's gate is split: 7a and 7c, then Phase 8; 7b (cluster) later.
- 7c on Xapiand local master `d25fb70a4`: S1 (tooling, baseline), X1 (Xapian and the cluster
  deleted), X2 and X2b (registry, turns, host mode, chores, reader views, per-index registries).
- Every 7c library step is on its main: task (L1), threadpool WorkPool (L2), http/reactor/http-log
  (L3), prospect sliced search (L4), strata `defer_io` (L5), storage streamed writes (L6), plus the
  compressors decoder fix.
- X3 (views on the reactors) is built and reviewed; one blocker (CLI `--restore` hangs at exit) in
  its fix round.
- The library pin wave of 2026-11-01 (cpp-libraries `docs/UPGRADING-2026-10.md`) is moving
  strata, prospect and Xapiand to the heads; the owner adopted boolean-parser's and datetime's
  result changes.

## Remaining, in order

| Step | What | Gate for | Review |
|---|---|---|---|
| X3 land | B1 fix, the pin wave rebased on it | X4 | lead confirms with the review's probes |
| X3-1 | strata's chunked per-index reader registry (shared-reader revision 4) | searches that suspend at scale | one light architect check (concurrency) |
| X4 | Lua: per-call `_ENV`, yielding hook, caps, the matcher patch (D10, C11-C13) | X6 | one code review |
| X5 | schema, resolver and control plane as coroutines (D11, D13); inventory J | X6 | developer's proof unless a new mechanism appears |
| X6 | delete the offload pools, threads and thread options (D4, D13, D15); destructor asserts; shutdown order (D16); `async_check.sh --strict` in CI | X7 | lead's skim |
| Perf | sliced search's 1.4-2.2% one-slice cost; the keep-alive watch byte's extra read | T12 | performance engineer |
| X7 | the lead runs T1 to T13 on master; gate report `docs/gates/phase7c.md`; Linux and FreeBSD runs owed since 7a (C5) | Phase 8 | architect gate review |
| Phase 8a | replace `sortable_serialise` (GPL) with the 8-byte map (phase-4 D3a) in Xapiand's MIT code; no Xapian reference left; license audit of every dependency (RapidJSON's patch included) | release | security reviewer for the audit |
| Phase 8b | closing benchmark, once: strata against Xapian 2.1.0, and Xapiand alpha.10 (Xapian) against Xapiand on strata end to end, every axis | release | none (published numbers, `quiet exclusive`) |
| Phase 8c | the rename (owner picks the name after a trademark search); changelog; tag; Homebrew; docs site | release | owner |

Before release, also: the barrier warning on non-Apple storage; the docs upgrade page from
alpha.9 and alpha.10 data; the postings fuzz target's memory growth; Detent off http's removed
offload path (coordinated with its agent).

## The strata lane, beside it

1. Cold-tier item 5 (`demote()`, `promote()`, `map_owed`) is built and proven on
   `phase/cold-tier-item5`; one review, then land.
2. Cold-tier step 2: the catalog, CT-R23's read backstop, the T2d wake-cost row.
3. Then the hybrid-search go/no-go run (owner, 2026-10-28): C1 rerun with strata at 1,000-vector
   tenants plus optimization experiments.

## Estimate

About four to five weeks to the release at this week's pace, with the weekly quota as the binding
limit: X3 and the pin wave this week; X4, X5 and X3-1 next; X6, the perf items and X7 after; Phase 8
the last one to two weeks. 7b (the cluster) follows the release.

## Decisions owed by the owner

- The product name (Phase 8c).
- Deleting the 66 archived branches (`branch-triage-2026-10-30.md`).
- `--max-indexes-per-search`'s default (the pool size, 200).
