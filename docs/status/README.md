# Workstreams

The lead's index (the gated pipeline in `docs/PLAN.md`): every live workstream, its
branch and worktree, who drives it, its state, and its next step. Each implementer keeps its
own ledger at `docs/status/<workstream>.md` on its branch; after a lost session, read this
file, then each ledger and its branch's `git log`.

## Current (2026-10-25)

Priority (owner, 2026-10-22): the write buffer first; buffer-internal optimizations wait for it;
other work is orthogonal and continues.

| Workstream | Repo | Branch | Worktree | Agent | State | Next | Updated |
|---|---|---|---|---|---|---|---|
| Write buffer: scan tail | strata | `perf/lanebuf` (pushed as `perf/lanebuf-rebased`) | `strata-wt/lanebuf` | done | owner accepted C at 1.5–1.8x RocksDB scan p99.9 at full write speed (2026-10-26) | C's scan p99 (5.6–24 ms at full write speed) against T3: E1, E2, then up to five experiments; fallback A' for scan-heavy indexes; ledger `docs/status/lanebuf.md` | 2026-10-25 |
| Shared reader: views | strata | landed | — | — | landed `23e78bb` (2026-10-26) | follow-ups CR5–CR10 and the `install()` truncation hazard (reachable; fix: reopen installed segments from the target's own files) | 2026-10-26 |
| Write buffer: step 1 (items 2–3) | strata | landed | — | — | landed `356022d` (2026-10-26) | follow-ups CR7–CR9 and the release-build second-writer note (`docs/status/write-buffer.md`) | 2026-10-26 |
| Write buffer: step 1 (item 4) | strata | landed | — | — | landed `2f56d64` (2026-10-26) | code review CR1, CR2 closed by re-check; R1 (budget charge) fixed; follow-up N1 (writerless runs, step 2) in `docs/status/write-buffer-reads.md` | 2026-10-26 |
| Write buffer design | strata | `design/write-modes` | — | — | revision 4 (`8cdbe61`), approved with conditions; step 1 built on it | conditions carried into steps 2–3 | 2026-10-26 |
| Shared reader design | strata | `design/shared-reader` | `strata-wt/shared-reader` | designer (idle) | revision 2 | used by the views step | 2026-10-21 |
| Xapiand engine core | Xapiand | `master` (local, unpushed) | `Xapiand` | lead | landed locally (`566943e9e`) | CR1, CR2 before the offload pool turns on; CR3, CR4, CR8, CR9 owed (`docs/status/engine-core.md`) | 2026-10-22 |

### Queued (in order)

1. T11a/T11b, the Xapiand-shaped run that decides multi-writer against the fallback (lead, after the scan fix).
2. Build the write buffer (developer, concurrency tier).
3. ~~Background deletes~~ landed `45e6b32` (2026-10-26): run phase 1.16× faster than main on 64 KiB overwrites.
4. ~~Xapiand pool bugs (CR8) and the offload pool's CR1/CR2~~ merged on Xapiand's local master (pool concurrency, offload bridge, follow-ups; unpushed).
5. ~~The engine query step (X3)~~ merged on Xapiand's local master (`1ab0ce077`, both steps, reviewed). L1 queryparser: steps 1–2 landed (`queryparser:b732d09`), step 3 (T2 on V2, juxtaposition grammar) building. X4 complete on Xapiand's local master (`cca0011bb`); gate 0.5 (X5) running.
6. termdict FST validation at open; the hashes 32-bit fold collision; the flush-memory bound that flakes under ASan (`flush_memory_test.cc:393`).
7. The turn log; move the cpp-btree pin once Detent publishes its fix.
8. Explorers on `docs/perf/hunt-list.md` targets.

## As of 2026-10-17 (stale: verify each row against its ledger before acting)

| Workstream | Repo | Branch | Worktree | Agent | State | Next | Updated |
|---|---|---|---|---|---|---|---|
| Phase 3 gate | strata | main | `strata` | lead | measuring | dry run clean; `night_gate.sh n1017` starts 23:00 under `quiet_trials.sh` (strict hold); five-trial query rows the next night; then the gate review | 2026-10-17 |
| Format 13 | strata | `phase4/format13` | `strata-wt/format13` | contributor | designing | ledger `docs/status/format13.md`; draft `docs/design/format-13.md`, then the architect's preliminary review; implement the stable decisions | 2026-10-17 |
| Layers refactor | strata | `phase4/layers-refactor` | `strata-wt/refactor` | developer (paused) | implementing | through step 7b at `707066b`, CI green; revision 3 re-checked APPROVE WITH CONDITIONS: R12, R17 to R19 before the full review, C5's COUNT and peak-RSS reruns before step 3 lands | 2026-10-17 |
| Xapiand failover | Xapiand | `fix/failover` (local) | `Xapiand-wt/failover` | contributor | fixing | `8a5bd916a` (promote the best live replica) passes FAILOVER=1 cluster_check 3 three times of three; `c51da3e09` (handler exceptions) unbuilt; the full matrix after the gate run; owner: accept promoting an empty replica? | 2026-10-17 |
| Xapiand HTTPS (D1) gate | Xapiand | `tls/d1-gate` (local) | `Xapiand-wt/tls-gate` | contributor | implementing | T1, T2 pass (`7e8cccb7d`); T3 probe fix, C5 counters, C6 daily expiry, T4 harness, gate report; the lead measures T4 and pushes for T5 | 2026-10-17 |
| Track D1 simulator | strata | `design/simulator` | `strata-wt/simulator` | contributor | implementing, in preliminary review | design at `e305adb`, S1 engine at `b70f2b4`; the architect reviews (`simulator-design.md`) while S3 (checker) and S2 (network) proceed; owner's Q1 to Q6 after the review | 2026-10-17 |
| Phase 5 automata | strata | `phase5/automata` | `strata-wt/automata` | contributor | designing | ledger `docs/status/automata.md`; `docs/design/automata.md`; preliminary review; implement with brute-force checks | 2026-10-17 |
| PKI helper | Xapiand | `design/pki` (local) | `Xapiand-wt/pki` | contributor | design cleared | revision 1 approved with conditions by the architect (PKI-R19 to R26) and the security reviewer (SR14 to SR17); done criteria K1 to K8 set; step 1 starts once the host is free, since Xapiand has no CI until pushed | 2026-10-17 |

## Priority: a solid, layered, ultra-fast, hyper-efficient engine (owner, 2026-10-17)

strata is two layers: the lifecycle core (a key-value engine that must beat RocksDB) and the
search plugins (which must beat Lucene and Tantivy), on all four axes: time, memory, disk, and
CPU per operation. Quota goes to that first; everything else waits for spare capacity.

1. Indexing to 4.5x the fastest baseline (PLAN, "Design target"):
   - `perf/inverter`: the tuple inverter (research C) in the index path, then flush_from's per-term cost.
   - `perf/small-indexes`: the many-index workload (`small-indexes.md`): cost per open index,
     aggregate throughput with many indexes writing. `perf/indexing-gap` (one index on eight
     threads) is parked: thousands to millions of indexes per host use their cores across indexes.
   - `perf/index-size`: reclaim merged-away sources (44% of T4's bytes), final merge, copy merges.
   - `research/indexing`: done; `docs/research/indexing.md` is the design.
2. The layers split (`phase4/layers-refactor`): R12 and R17 to R19, then steps through 7, so the
   core and the plugins are separate; format 13 (its re-check, the copy-merge amendment, 7c) rides on it.
3. The core against RocksDB: calibrate db_bench's fillseq, fillrandom, readrandom, and seekrandom
   and YCSB A to F once, at about 1M keys (the `document` profile gate, `rocksdb-survey.md`), then
   time strata's core against RocksDB only, targeted, as the core takes shape.
4. Query speed: query-gap revision 2's re-check (D17), E1's tooling, E0, then the E items with a
   performance engineer, against the fastest per class.
5. Correctness gates on the engine path: code reviews of the perf branches before they land,
   automata.

Parked until engine work has spare capacity: peer transport revision 3, the simulator's SIM-R20 to R27,
Xapiand's HTTPS, failover, and PKI reviews, bisection's code review.

## Queued

Paused on 2026-10-17 at the usage limit, to resume as `quota advise` allows, in this order (each
from its ledger, as the agent type named):

0. Indexing gap (`performance-engineer`, first when the host frees): T4's first two one-thread
   rounds put strata at about 0.46x Tantivy's throughput (19.7 and 18.4 min against 8.8 and 8.7;
   Lucene 14.9), against Phase 3's 1.0x of the faster and Phase 2's passed 0.8x. Attribute before
   changing code: A/Bs with keys off, a larger budget, and background merges off, then a profile.
0b. Query gap (`designer` for revision 1, then `performance-engineer`): reviewed APPROVE WITH
   CONDITIONS (`design/query-gap`); Q1 to Q3 decided in its ledger. Before any item enters
   format 13's selection: QG-R1 (M17's tighter bound), R2 and R3 (a checked frontier invariant,
   P17 re-sorting copied frontiers), R5 (a 64-bit sum in M23), R6 (a byte budget). M18 is out.
   After E0 and E1, E9 (two-pass counting for 3+-clause unions) goes first.
1. Format 13 (`designer`, then `developer`): preliminary review APPROVE WITH CONDITIONS (`4266374`);
   revision 1 answers F13-R1 to R5 and R11 before code returns to the reader, writer, index, or
   merge, and F13-R7 and R8 before F10. Q1 and Q2 decided in PLAN (keyed `-bp`; reserve P8, P14,
   P18, P20). It rebases onto `phase4/layers-refactor`
   at its tip `707066b` (SYS-1, decided by the lead; `c8c03f6` became `8645134` in layers' rebase); until then it stays out of `reader.hh`, `writer.hh`,
   `index.hh`, and `merge.hh`.
2. Track D1 simulator (`developer`): CI on `7d0b3fa`, then S4 once its review lands.
3. Xapiand HTTPS gate (`developer`): T3, C5, C6, the T4 harness, the gate report.
4. Phase 5 automata (`developer`): the design's preliminary review, then the rest of the build.
5. Layers refactor (`developer`, paused; the critical path for format 13; next R12, R17 to R19, then C5 under a hold): rebase, CI, the
   format-13 findings that belong to layers (F13-R2, R3, R11), then steps through 7.
6. Bisection on the shared pool (`developer`, branch `phase3/bisection-pool`): `bisection.hh` spawns
   its own `std::thread` workers, a second pool beside Kronuz/threadpool's, which strata already
   links (`pool_executor.hh`). Submit its parts as a `TaskGroup` capped by `presets::threads`;
   avoid the nested-wait deadlock when a merge already runs on a pool worker (help while waiting,
   or a separate small pool, chosen by a test that saturates the pool); any threadpool change is
   additive (Detent links it); T7's pinned hash stays identical at 1 and 8 threads. Code review
   before landing.
7. Track C T6 (`performance-engineer`, after the gate run): fsyncs per second at 1,000 groups for
   the per-group and shared journal backends, deciding SYS-8 with Detent's maintainer.
8. Peer transport design (`designer`): Track C's D16 extended with SPKI pins and an N-1 version
   range (SYS-2, SYS-3), reviewed by the architect and the security reviewer.
8b. Peer transport revision 1 (`designer`): both reviews approve with conditions (PT-R1 to R19,
   SR1 to SR14; Q1 to Q5 decided in its ledger). Steps 1 to 3, 5, 6 may start after; step 4
   waits for the re-check. Detent items to share: Q4's restart, PT-R8 (sessions closing at
   notAfter, `PeerConfiguration` built by position).
9. Main CI chores (`maintenance-engineer`): the ASan+UBSan job hits its 60-minute limit before
   the slow tests (cpp-libraries' shared `cmake.yml` hard-codes `timeout-minutes: 60`; make it an
   input and give strata's sanitizer jobs more), and land format 13's `keybench` on main so format
   12's key-lookup baseline for T5 is recorded.
10. reactor UDP receive loop (`developer`, in `~/code/Kronuz/reactor`): the loop exits for good
   when a handler throws, so a failover right after the first write never elects a leader
   (found by Xapiand failover, `c51da3e09`). The fix must be additive, since Detent links reactor.

The gate VM run (`night_gate.sh n1017`, started 13:17) holds the host strictly until about
01:17; resumed agents draft and push without compiling until it ends.

## Landed

- P1 reordering: on main through `950d24d` (T1 to T7 recorded in `docs/gates/phase-3-ratchets.md`
  and `docs/design/reordering.md`).
- P6 multi-segment search: every commit of `phase3/p6-multisegment` is on main (`9e016ca` through
  `94fffcc`; `5c199dd` is `8fefcb4`, the same diff on newer context). Branch and worktree removed.
