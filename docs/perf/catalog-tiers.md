# The cold tier's gate rows through the catalog

`bench/catalog_tiers` measures `docs/design/cold-tier.md`'s T1 to T4 and T2d as a host runs the
catalog: a `catalog<kv_index>` over `PosixIO` directories that `idle_index build` makes, a
`work_pool_executor` (eight chores at once), a timer thread, one shared `reader_registry`, and the
process's real `resource_budget`, which raises `RLIMIT_NOFILE` to its hard limit and caps from it.
`bench/catalog_tiers/tiers.sh` builds this tree's harness beside `main`'s `idle_index`, the
re-measured baseline (A3), and runs each row with the baseline interleaved in the same job.

## How each row runs

- **Tiers.** Hot is every index opened by `try_pin` (eager segments, so every segment mapped);
  warm is `begin_demote()` on each slot (`open` with nothing mapped, or `owed` under `defer_io`);
  cold is demoted then `begin_close()`d, so no slot holds it. The sweep is not started, so nothing
  moves a slot between forcing and measuring.
- **T1** meters the process as `idle_index` does (resident, footprint, descriptors, mappings)
  after a 5 s idle, and the counting build (`catalog_tiers_count`, `bench/idle_index/heap_count.hh`)
  gives the live heap per index.
- **T2 and T2d** take one request pin and one `get` per index in a random order from one thread off
  the pool. A warm `defer_io` index's pin starts the wake, `promote()` runs on the pool, and the pin
  resumes on the requesting thread; a cold one's pin posts the open to the same pool. Under
  `defer_io` the run then times `promote()` alone on 10,000 indexes opened without the catalog,
  per segment, against D3's 33 to 50 µs estimate.
- **T3** starts the sweep with the row's caps, gets every index once on 8 threads, then 1M Zipf
  requests at θ = 0.99. A sampler reads the kernel every second: mappings and descriptors, and on
  macOS this process's vnodes (the distinct files behind its descriptors and mapped regions;
  `kern.num_vnodes` is the system's vnode cache, which sits at `kern.maxvnodes`, 263,168 here, on a
  busy host, so it is reported, not compared). The harness prints PASS on zero `ENFILE`, `EMFILE`,
  `ENOMEM` and `too_many_open`, every peak below the budget's hard caps, and open and hot counts
  within one pass (`pass_posts`, 16) of the soft caps 3 s after the requests end. The design's
  target is every sample after the touch ends; the verdict below reads the samples against it.
  Each sample also prints a `flow` line: the slots in each transition (begun, chore queued or
  running), the relief booked, the open and hot counts net of both, and the passes, reposts,
  demotes and closes of the last second.
- **T4** replays `test/s3fifo_model.hh`'s trace (mt19937_64 seeded 42; the scan variant inserts one
  access to every key after the warm-up) on 8 threads from one cursor, each request a pin by key
  and a re-pin by handle. The hit ratio over the measured part is one less the misses (opens of an
  absent key, reopens of a closing one, retries) over the requests; the models (`s8_variants` and
  `model_total`) replay the same trace at the catalog's `soft_open`. The row passes at the
  `s8_variants` ratio less one point with no kernel error.

## Commands

Each row is one strict job of at most 10 minutes holding its interleaved pairs, whose spread is
the interval (D59); the counts are scaled to fit, and the caps scale with them for T3 and T4.

```sh
tiers.sh build OUT MAIN_SRC 0                # light: binaries only (0 keeps the fixtures)
tiers.sh t1 OUT d1s1 1000 2                  # T1, with d1000s1; d100s10 and d1000s10 a second job
tiers.sh t2 OUT d1s1 500 3                   # T2/T2d, one job per fixture (d1000s1, d1000s10)
tiers.sh t3 OUT 50000 100000 2000 500        # T3: 25x the open cap, 100x the hot cap
tiers.sh t4 OUT 0.99 noscan 20000 40000 20000 1000 250   # T4: two cells per job
tiers.sh t2hot OUT d1s1 1000 8               # T2 hot A/B, exclusive: order alternating per pair
tiers.sh t1pieces OUT d1s1 1000 warm         # T1's pieces: live allocation sites, named by atos
```

At full scale (milestone checks, run once on main): `tiers.sh build OUT MAIN_SRC 100000
1000000`, `t1 OUT FIXTURE 100000 3`, `t2 OUT FIXTURE 100000 1 [drop]`, `t3 OUT 1000000`, `t4 OUT
THETA [scan]` with the platform's caps.

A one-document index has one segment, so the 10-segment fixtures carry 100 and 1,000 documents.

## Findings while building it

- **Every open syncs twice.** `basic_index::open` syncs the directory and then the manifest log
  (`index.hh`, the rollover's D6.6 rule), on `main` too. On the development Mac under other jobs'
  load, a sample of `idle_index open` spent nearly all its time in those two `fcntl` calls, and
  opens took 25 to 60 ms each. T2's cold row, T3's touch and T4's misses pay it per open.

## Results

macOS x86_64 (the development Mac), cache warm, strict jobs at strata:9621595f with main
strata:15d39c30's `idle_index` interleaved. Each range is the spread of the job's pairs (D59).

**T1, memory per tier** (261111-035757-efe281 one segment, 261111-035757-083bc8 ten; 1,000
indexes, 2 pairs). Heap per index from the counting build is exact; RSS per index at 1,000
indexes carries about 5 KB of process baseline, so it is not comparable with the 5.9 KB target
set at 100,000.

| Fixture | main heap | hot | warm | cold | hot RSS vs main | maps per index hot / warm / cold |
|---|---|---|---|---|---|---|
| d1s1 | 6,484 B | 7,134 B | 5,084 B | 292 B | 12,669-12,685 vs 11,612-11,616 B (+9%) | 1.03 / 0.03 / 0.03 |
| d1000s1 | 6,849 B | 7,499 B | 5,204 B | 292 B | 21,271-21,291 vs 20,124-20,161 B (+6%) | 1.03 / 0.03 / 0.03 |
| d100s10 | 27,798 B | 30,248 B | 11,396 B | 292 B | 73,613-73,683 vs 70,611-70,840 B (+4%) | 10.04 / 0.03 / 0.03 |
| d1000s10 | 28,058 B | 30,508 B | 11,476 B | 292 B | 114,745-114,749 vs 111,800-112,157 B (+3%) | 10.03 / 0.03 / 0.03 |

- Hot through the catalog costs 650 B of heap per index over main's open with one segment and
  2,450 B with ten (5 and 23 allocations more). `idle_index`'s own hot, without the catalog,
  matches main exactly (K6 below).
- Cold keeps 292 B and 1.08 allocations per index: the catalog's entry for the key. T1's "cold
  ≤ 16 B" is not met at step 2.
- defer_io changes no row. Warm holds one descriptor per index and no map.
- The 10-segment fixtures were built with 10 segments by this tree's `idle_index` (main's
  ignores SEGMENTS).

**K6, T1's step-1 rows** (261111-035757-a253be; `idle_index` alone, 1,000 indexes, 2 pairs; its
binary is the one built at strata:dbfc1898, whose diff to strata:9621595f is `reader.hh`'s stored
frame decode, which an open does not reach): hot's heap equals main's on every fixture (6,484,
6,518 and 27,798 B on d1s1, d100s1, d100s10), and its RSS is within -0.0% to +3.3% of main's.
Warm: 4,798, 4,806 and 11,110 B of heap, 0.017 to 0.026 maps per index. Lazy: 4,826, 4,834 and
11,138 B, 0.005 to 0.022 maps.

**T2 and T2d, first query per tier** (500 indexes, 3 pairs; p50 and p99 in µs, the range over the
pairs):

| Fixture | main open | main query | hot | warm | warm defer (T2d) | cold | cold defer |
|---|---|---|---|---|---|---|---|
| d1s1 | 930-20,882 / 44,036-57,075 | 0.9-1.5 / 1.5-2.5 | 1.3-2.1 / 1.9-2.9 | 33-47 / 74-96 | 47-85 / 108-180 | 212-21,244 / 42,060-59,913 | 264-20,946 / 42,224-59,607 |
| d1000s1 | 1,265-41,108 / 42,142-58,753 | 5.1-9.0 / 6.1-18 | 5.9-9.3 / 9.3-18 | 43-58 / 64-96 | 54-71 / 135-188 | 20,868-20,955 / 41,995-56,980 | 379-41,137 / 56,074-72,264 |
| d1000s10 | 2,527-22,037 / 42,345-50,817 | 5.7-6.4 / 7.8-19 | 5.9-7.7 / 9.0-15 | 338-344 / 509-579 | 351-394 / 501-799 | 972-21,901 / 43,610-57,882 | 1,097-21,908 / 42,923-57,516 |

- Jobs, one strict job per fixture: 261111-085642-fdea98 (d1s1), 261111-085644-609714
  (d1000s1), 261111-035757-f9dbcf (d1000s10).
- A5 on macOS, cache warm: the wake (warm defer) is below cold at p50 and p99 on every fixture.
  No interval overlaps: the slowest warm defer is below the fastest cold at p50 and at p99. The
  margin at p50 is smallest in a pair whose cold opens ran fast: 3.1 times on d1s1 (264 against
  85 µs), 5.3 times on d1000s1 (379 against 71 µs) and 2.8 times on d1000s10 (1,097 against
  394 µs); in the other pairs it is 60 to 760 times. `promote()` costs 90 to 116 µs at p50 with
  one segment and 56 to 63 µs per segment with ten (134 to 239 µs p99), above the design's 33 to
  50 µs estimate.
- The margin rests on the open's two syncs (Findings): cold's p50 is 20 to 41 ms because of them,
  and where they ran fast cold was 212 to 1,097 µs and the margin 2.8 to 5.3 times. If the sync
  deferral lands, T2d is rerun before D3's latency sentence stands on it.
- An open costs 21 to 41 ms at p50 in most pairs and about 1 to 2.5 ms in a few: the two syncs of
  every open (above) dominate it.

**T3, a count far above the caps**: 50,000 one-document indexes against soft_open 2,000 and
soft_hot 500, a touch of every index then 100,000 Zipf 0.99 requests at 8 threads.

| Job | Commit | Samples after the touch over soft+16, open / hot | Max after the touch, open / hot | Passes per s (started, reposts) | Zipf requests/s, p50 |
|---|---|---|---|---|---|
| 261111-034629-8f5208 | strata:9621595f | 212 / 167 of 248 | 2,355 / 811 | not traced | 355, 197 µs |
| 261111-113116-5b7a23 | strata:f3f7d9c2 | 171 / 194 of 228 (net of relief) | 3,823 / 2,034 net, near the touch's end | 19 (1, 18) | 375, 175 µs |
| 261111-113856-358d4c | strata:5d3c948a | 0 / 0 of 278 | 2,012 / 512 | 62 (62, 0) | 311, 391 µs |

- **Why the caps were missed** (5b7a23's `flow` lines, Zipf medians): the excess was not relief in
  flight (0 slots demoting or closing, net open 2,043 and net hot 572, equal to the counts). One
  pass per second started (passes less reposts), the timer's: the catalog's own soft caps had no
  trigger, unlike the budget's caps, which post at their crossing. Each timed pass reposted about
  18 times until its demotes and closes covered the excess, then the chain ended, and a second of
  misses (112 opens/s) and maps (74 wakes/s) piled above the caps before the next timed pass.
- **The fix** (strata:5d3c948a): a miss and a last unpin post a pass when soft_open or soft_hot is
  over net of the relief in flight and no pass is queued. With it every sample after the touch is
  within soft+16, from the first; the whole run, touch included, peaks at open 2,022 and hot 523
  (4,905 and 3,241 before). Passes rise from 11,584 to 41,969 in the run, each started by a crossing
  (no reposts). Zipf ran at 311 against 375 requests/s with p50 391 against 175 µs in single runs:
  holding hot at 501 rather than 572 sends more requests through a wake, queued on the same eight
  chores as the opens' syncs; one run each, not an A/B, so the throughput change is not attributed.
- Zero `ENFILE`, `EMFILE`, `ENOMEM`, `too_many_open`; peaks below the hard caps in every run (358d4c:
  2,533 descriptors, 659 mappings, 3,056 vnodes against hard caps of 995,174 descriptors and
  157,900 vnodes). The sampler reads the budget, then the kernel (`PROC_PIDLISTFDS` and the whole
  `mach_vm_region` walk), then the budget again, and reports the kernel's descriptors beyond the
  larger read: at most 11 (8f5208), 8 (5b7a23), 4 (358d4c). The steady excess is 3, the stdio
  descriptors; the rest are short-lived descriptors opened inside the window, at most one per
  concurrent chore (eight), so the bound grows with the chores' concurrency and the window's width,
  not the index count (the earlier 1,050 were the sampler's own skew, `fd-budget.md`).

**T4, hit ratio against the S3-FIFO models** (261111-035757-a2d41c, 261111-035757-bbb067):
20,000 indexes, soft_open 1,000, soft_hot 250, 20,000 warm-up and 40,000 measured requests.

| Theta | Scan | Hit ratio | s8_variants | Gate (model less 1 point) | Hottest key pin+unpin, by key / by handle |
|---|---|---|---|---|---|
| 0.99 | no | 0.6824 | 0.6753 | PASS | 4,525 / 648 ns |
| 0.99 | yes | 0.6806 | 0.6751 | PASS | 4,394 / 603 ns |
| 1.2 | no | 0.8735 | 0.8717 | PASS | 5,097 / 548 ns |
| 1.2 | yes | 0.8735 | 0.8713 | PASS | 4,031 / 738 ns |

No kernel error and no `too_many_open` in any cell.

## Verdicts against the design's targets

Each from the jobs above, macOS, cache warm, scaled under D59. T2's are judged pair by pair
within a job (an open plus `get` as the sum of their p50s and of their p99s).

- **T1 hot, within 5% of the baseline:** MET through K6 (A9), the design's procedure: heap equal
  to main's, RSS -0.0% to +3.3%. Through the catalog hot costs 650 B (+10%) of heap with one
  segment.
- **T1 warm, the sum of its pieces plus 7%, 0 maps:** 0 maps MET (0.03 per index, equal to cold's:
  the harness's own regions). Heap MISS (261111-113121-6aaa2d, d1s1, 1,000 indexes, the live
  allocation sites after the demotion, named by `atos`): the design's pieces are `basic_index`
  1,840 B, the empty buffer 920 B, the view 208 B, the record 200 B with its bits and control
  block 48 B, the reader state 144 B, the publication 136 B and the slot's share 155 B (its chunk
  141 B, the key table 14 B): 3,651 B, plus 7% 3,907 B, against 5,022 B measured in the same run
  (5,084 B in the tier run): 38% over the sum, 29% over the bound. The rest, 1,360 B, is pieces the design's list leaves out:
  the manifest and file maps 544 B, the view's own allocations 256 B (its segment vector, the
  publish, the field table), the host's IO 256 B (`PosixIO`, the counting wrapper, the owner
  lock), the merge machinery 224 B (`start_merges` and its executor group) and `basic_index`'s
  constructor 80 B. Either the list grows by those or they go.
- **T1 cold, ≤ 16 B, 0 fds, 0 maps:** MISS: 292 B per index (the catalog's entry for the key);
  0 fds and no map of its own met.
- **T2 hot on d1s1, within 5% of main at p50 and p99:** MISS, beyond the noise
  (261111-113118-6d8a9c, exclusive, 1,000 indexes, 8 pairs, order alternating per pair, at
  strata:f3f7d9c2): hot's p50 is 1.5 µs against main's 0.9 µs (medians over the pairs), +25% to
  +144% per pair, median +71%; the A/A spread is 0.7 to 1.2 µs for main and 1.4 to 2.2 µs for hot,
  so no pair's hot reaches any pair's main. p99 3.05 against 1.8 µs (medians, +69%); main's p99
  ran 22 to 25 µs in two pairs, the only ones where hot was faster. The difference is the request
  pin by key and its unpin, about 0.6 µs, which main's bare `get` does not pay.
- **T2 cold, within 10% of open plus `get`,** judged only on pairs whose opens ran in the same
  sync mode (an open is about 1 ms, 21 ms or 41 ms by how many of its two syncs wait on the
  device; a pair split across modes measures the device, not the tier):
  - d1s1: MET. All three pairs share a mode: -77% (both fast), +1.7% and +0.2% at p50; +5.0%
    and +2.2% at p99 in the slow pairs.
  - d1000s1: MET on its one same-mode pair (2: 20.9 against 21.0 ms): -0.3% at p50, -1.2% at
    p99. Pairs 1 (main 1.3 ms, cold 20.9 ms) and 3 (main 41.1 ms, cold 21.0 ms) are split.
  - d1000s10: p50 MET, p99 MISS as read, on its one same-mode pair (2: 22.0 against 21.0 ms):
    -4.5% at p50, +13.9% at p99 (50.8 against 57.9 ms). Main's own p99 over its two slow-mode
    pairs spans 45.0 to 50.8 ms (13%), so 500 indexes do not resolve 10% at p99. Pairs 1 (main
    2.5 ms, cold 21.9 ms) and 3 (main 21.9 ms, cold 1.0 ms) are split.
- **T2d, warm below cold at p50 and p99:** MET on every fixture (A5 above).
- **T3, within one sweep pass of the soft caps after the touch ends:** MET at strata:5d3c948a
  (261111-113856-358d4c): 0 of 278 samples after the touch over soft+16, maxima open 2,012 and hot
  512, within from the first sample after the touch. Before the soft-cap trigger MISS (8f5208:
  212 and 167 of 248 over, maxima 2,355 and 811, within only once the requests ended, about 282 s
  after the touch); the cause, measured, is above.
- **T3, zero kernel errors and `too_many_open`, peaks below the hard caps:** MET.
- **T4, hit ratio at least the model's less 1 point:** MET in 4 of 4 cells at strata:9621595f.
  The soft-cap trigger changes when the sweep evicts; T4 reruns at strata:7b126bc1
  (261111-123758-7ea903, 261111-123800-7a33ff).
