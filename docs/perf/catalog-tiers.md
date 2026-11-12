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

**The trigger's cost, an A/B**: `tiers.sh t3ab`, 50,000 indexes, 12,000 Zipf requests, the same
caps, one pair per exclusive job, order alternating by job. `tiers.sh buildab` builds both sides
from one tree, off taking strata:f3f7d9c2's `catalog.hh`, so the catalog alone differs (on is the
one-slot trigger, strata:7b126bc1). Waited: requests whose pin waited for a transition; owed: those
that met an owed slot, a wake. The flush probe is a 4 KiB write and one `F_FULLFSYNC` every 200 ms
under the fixtures during the Zipf phase.

| Job | Side | Zipf req/s | p50 | Waited (owed) | Waited p50 | Flush probe p50 | Passes | T3 |
|---|---|---|---|---|---|---|---|---|
| 261111-143023-ac195f | on | 253 | 1.46 ms | 60.2% (2,268) | 62.8 ms | 52.0 ms | 26,368 | PASS |
| | off | 303 | 0.01 ms | 46.7% (730) | 62.9 ms | 52.3 ms | 7,001 | FAIL (hot 1,388 of 500) |
| 261111-143025-7e33e6 | off | 276 | 0.64 ms | 55.9% (1,810) | 62.0 ms | 55.0 ms | 6,956 | PASS |
| | on | 216 | 4.64 ms | 60.1% (2,248) | 70.1 ms | 62.8 ms | 29,345 | PASS |
| 261111-143034-071676 | on | 256 | 1.57 ms | 59.5% (2,205) | 61.3 ms | 58.2 ms | 27,944 | PASS |
| | off | 320 | 0.66 ms | 57.2% (1,984) | 41.8 ms | 53.3 ms | 6,825 | PASS |
| 261111-143035-141fc0 | off | 416 | 0.54 ms | 58.7% (2,251) | 21.0 ms | 51.7 ms | 6,877 | PASS |
| | on | 303 | 1.06 ms | 58.3% (2,050) | 41.8 ms | 51.8 ms | 24,872 | PASS |

- **The one-slot trigger costs Zipf throughput beyond off's spread.** On ran slower in 4 of 4
  pairs, at 0.83, 0.78, 0.80 and 0.73 of off (mean 257 against 329 req/s). On's best run (303)
  only ties off's worst (276 to 416). 261111-124104-418363 (strict, 4 pairs, one job, on against
  f3f7d9c2's whole tree) agrees in 3 of 4 pairs (0.91, 0.96, 1.10, 0.80).
- **The device ran in its slow mode in every run, on and off alike** (probe p50 52 to 63 ms, every
  flush over 5 ms, `open-sync.md`). The device sets the size of the step, not which side is
  slower.
- **The 19.4 and 19.7 ms p50s are the trigger's, through the median's step.** A request finds its
  index open (4 µs median), waits for a wake (p10 0.4 to 1 ms), or waits for an open that pays a
  slow-mode flush (20 ms and more). Waited shares sit near 50%, so the p50 lands on either side
  of a 20 ms step. On holds the waited share at 58 to 60% (off: 47 to 59%), so its median sits
  higher: 1.1 to 4.6 ms here against 0.01 to 0.66 ms for off, and 19.4 and 19.7 ms in two of
  418363's runs.
- **Where the time goes: the passes** (the band trigger below settles it). Opens are equal (4,876 on, 4,730 to
  4,863 off) and so are demotes and closes (about 56,500 and 52,870). On runs 3.6 to 4.2 times the
  passes (each started at a one-slot excess, relieving one or two slots) and up to about 1,500
  more owed wakes. Off's fastest run had as many wakes as on (141fc0: 2,251), which points at the
  passes rather than the wakes.
- 261111-134927-7430a0 (3 pairs in one strict job) hit its 10-minute bound after pair 2, so it
  is not evidence. Its two pairs favored on (397 and 348 against 260 and 293 req/s).
**The band trigger** (strata:062182b2, the one experiment): a miss or last unpin posts a pass only
once the excess net of relief reaches `pass_posts` (16), so each triggered pass does a full pass's
work. Same A/B, off unchanged; build 261111-151532-1f0913.

| Job | Side | Zipf req/s | p50 | Waited (owed) | Waited p50 | Flush probe p50 | req/s x flush p50 | Passes (started) | T3 |
|---|---|---|---|---|---|---|---|---|---|
| 261111-151536-71d322 | on | 368 | 0.48 ms | 57.3% (1,947) | 22.5 ms | 44.8 ms | 16.5 k | 9,171 (2,308) | PASS |
| | off | 239 | 0.89 ms | 55.3% (1,938) | 66.0 ms | 65.4 ms | 15.6 k | 6,985 (49) | PASS |
| 261111-151540-9ba721 | off | 336 | 0.66 ms | 60.4% (2,316) | 37.0 ms | 46.1 ms | 15.5 k | 7,008 (204) | PASS |
| | on | 284 | 0.88 ms | 57.9% (2,033) | 59.9 ms | 61.0 ms | 17.3 k | 9,527 (2,305) | PASS |
| 261111-151543-9397aa | on | 270 | 0.84 ms | 58.1% (2,050) | 60.9 ms | 61.8 ms | 16.7 k | 9,411 (2,310) | PASS |
| | off | 297 | 1.38 ms | 62.0% (2,501) | 41.9 ms | 54.1 ms | 16.1 k | 7,257 (91) | FAIL (hot 560 of 500) |
| 261111-151547-259305 | off | 299 | 0.55 ms | 55.4% (1,939) | 59.7 ms | 53.3 ms | 15.9 k | 6,823 (48) | PASS |
| | on | 260 | 0.87 ms | 57.8% (2,010) | 62.7 ms | 50.3 ms | 13.1 k | 9,120 (2,309) | PASS |

- **Throughput: within off's spread.** On ran 260 to 368 and off 239 to 336 req/s; the means are
  296 and 293. Pair ratios are 1.54, 0.85, 0.91 and 0.87 (median 0.89), so on trails in 3 of 4
  pairs. The first pair's 1.54 is not a position effect: the first run of a job was on in two
  jobs and off in two, here and in the one-slot round, with no consistent winner.
- **What the remaining spread is: the device's flush time per run, not the passes or the cap.**
  The opens are flush-bound (4,850 per run through 8 chores at a ~50 ms flush is about 30 s, the
  Zipf phase's length). Across the eight runs, req/s mostly falls as the run's flush probe p50
  rises, whichever side ran (on's 260 at 50.3 ms is the exception). Scaled by it (req/s x flush p50, a constant if the opens alone set the rate),
  on is 15.9 k against off's 15.8 k (+0.7%). The same scaling keeps the one-slot trigger's loss
  (14.3 k against 17.4 k, -18%), so it separates a real cost from the device's noise. What the cap
  forces is no larger on: opens 4,873 to 4,876 against 4,675 to 4,865, demotes and closes equal,
  waits on owed slots 1,947 to 2,050 against 1,938 to 2,501, waited share 57 to 58% against 55 to 62%.
  The passes the trigger adds fell from about 16,000 started per run to 2,300, each a full pass.
  Four pairs resolve about 10%: a cost under that is not excluded.
- **T3 at the band** (261111-151551-cd5253, 358d4c's command, 100,000 Zipf): PASS, 3 s after the
  requests open 2,000 and hot 487; 327 req/s. During the requests the counts sit at the band's edge:
  of 259 samples after the touch, 75 (open) and 23 (hot) are over soft+16 as counted, and 4 and 5
  net of the relief in flight. The maxima are 2,029 and 523 counted, and 2,019 and 519 (soft+19) net.
  The 3 past the pass are opens begun while the triggered pass waits for a chore (up to 8 in flight).
  The one-slot trigger had 0 and 0 of 278.
- Suite 261111-151520-66533e (74 of 74); mutant 261111-151526-29ff8a: the one-slot trigger fails
  `soft_caps_post`'s band check (3 over with a pass of 4 posts nothing).

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
  constructor 80 B. The lead's ruling (11-11): those pieces join T1's list in
  `docs/design/cold-tier.md` with their bytes; this verdict stays MISS against the old sum.
- **T1 cold, ≤ 16 B, 0 fds, 0 maps:** MISS: 292 B per index (the catalog's entry for the key);
  0 fds and no map of its own met.
- **T2 hot on d1s1, within 5% of main at p50 and p99:** MISS, beyond the noise
  (261111-113118-6d8a9c, exclusive, 1,000 indexes, 8 pairs, order alternating per pair, at
  strata:f3f7d9c2): hot's p50 is 1.5 µs against main's 0.9 µs (medians over the pairs), +25% to
  +144% per pair, median +71%; the A/A spread is 0.7 to 1.2 µs for main and 1.4 to 2.2 µs for hot,
  so no pair's hot reaches any pair's main. p99 3.05 against 1.8 µs (medians, +69%); main's p99
  ran 22 to 25 µs in two pairs, the only ones where hot was faster. The difference is the request
  pin by key and its unpin, about 0.6 µs, which main's bare `get` does not pay.
  Recorded as a residual (the lead, 11-11).
- **T2 hot on d1000s1, within 5% of main at p50 and p99:** MISS, a recorded residual
  (261111-085644-609714, 3 pairs, hot against main): p50 8.3/9.0 (-8%), 5.9/5.1 (+16%), 9.3/7.6
  (+22%) µs, over 5% in 2 of 3 pairs. p99 +52% and +80% in two pairs, below main in the third. It is not
  attributed to d1s1's request pin by key; no measurement splits it.
- **T2 hot on d1000s10, within 5% of main at p50 and p99:** MISS, a recorded residual
  (261111-035757-f9dbcf, 3 pairs): p50 7.6/5.7 (+33%), 7.7/6.4 (+20%), 5.9/5.8 (+2%) µs, over 5% in
  2 of 3 pairs. p99 +92% and +14% in two pairs, below main in the third. It is not attributed to the
  request pin either.
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
  512, within from the first sample after the touch. At the band trigger (strata:062182b2,
  261111-151551-cd5253) it is MET against the ruled bound (the lead, 11-11: soft + one pass of 16
  + the opens in flight at the sample) in 259 of 259 samples for open and for hot, the net counts of
  each `flow` line against its own `opening` (the reviewer's probe `t3_bound.py`). Net maxima are
  2,019 and 519. 4 and 5 samples sit past soft+16 by at most 3, opens in flight while the pass waits
  for a chore. At the requests' end the counts are open 2,000 and hot 487. Before the soft-cap trigger MISS (8f5208:
  212 and 167 of 248 over, maxima 2,355 and 811, within only once the requests ended, about 282 s
  after the touch); the cause, measured, is above.
- **T3, zero kernel errors and `too_many_open`, peaks below the hard caps:** MET.
- **T4, hit ratio at least the model's less 1 point:** MET in 4 of 4 cells at strata:9621595f.
  The soft-cap trigger changes when the sweep evicts; T4 reruns at strata:7b126bc1
  (261111-123758-7ea903, 261111-123800-7a33ff).
- **The soft-cap trigger costs no Zipf throughput beyond off's spread** (the lead's condition,
  11-11): MET at the band trigger (strata:062182b2): on 260 to 368 against off 239 to 336 req/s,
  means 296 and 293, +0.7% scaled by each run's flush time. The pair median is 0.89, inside the
  device's run-to-run spread. The one-slot trigger (strata:7b126bc1) was a MISS: 0.73 to 0.83 of
  off in 4 of 4 pairs, -18% scaled (the A/Bs above).
